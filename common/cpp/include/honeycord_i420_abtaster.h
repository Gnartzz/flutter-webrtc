// honeycord (16.11.2026): I420 direkt verkleinert nach RGBA (Byte-Reihenfolge
// R,G,B,A = libyuv „ABGR", genau das, was CopyPixelBuffer bisher ueber
// ConvertToARGB(kABGR) lieferte und Flutters PixelBuffer erwartet).
//
// ANLASS (AIX1, Radeon 890M, 16.11.2026): Flutter-AMD empfaengt einen fremden
// Bildschirm 3840x1080@60 in einer ~890 Geraetepixel breiten Kachel —
// CopyPixelBuffer brauchte 10,7 ms je Bild. Der Wrapper (libwebrtc
// rtc_video_frame_impl.cc, ConvertToARGB) kopiert zuerst das GANZE Bild
// (I420Buffer::Rotate, auch bei Drehung 0 eine volle Kopie in frisch
// angelegten Speicher), legt ein zweites I420 in Zielgroesse an, skaliert
// (ScaleFrom) und rechnet erst dann um. Die DLL neu zu bauen ist zu teuer;
// die oeffentliche API (rtc_video_frame.h) gibt aber DataY/U/V und
// StrideY/U/V her — hier wird daraus in EINEM Durchgang verkleinert und
// umgerechnet, ohne Kopie und ohne Allokation je Bild.
//
// Verfahren = libyuv kFilterBox (ScalePlaneBox), das der alte Weg in diesem
// Bereich ebenfalls nimmt: jedes Zielpixel ist der Mittelwert eines ganzzahligen
// Quellrechtecks (Spannen wie libyuv, Rundung hier korrekt statt abgeschnitten).
// Y in Zielgroesse, U/V in halber Zielgroesse (wie I420Scale die Farbebenen
// skaliert) und je 2x2 Zielpixel geteilt (wie I420ToABGR). Farbumrechnung
// BT.601 begrenzter Bereich mit den Ganzzahl-Koeffizienten aus libyuv
// row_common.cc (YG 18997, YB -1160, UB 128, UG 25, VG 52, VR 102, >> 6) —
// bitgleich zur C-Referenz von YuvPixel.
//
// Nur fuer deutliches Verkleinern (I420DirektGeeignet): libyuv nimmt den
// Box-Filter nur, wenn BEIDE Achsen um mehr als die Haelfte schrumpfen, sonst
// bilinear — dort bleibt der alte Weg, damit sich das Bild nicht aendert.
//
// C++14 (Linux baut das Plugin mit -std=c++14 -Wall -Werror), Windows mit
// /W4 /WX: keine stillen Verengungen.
#ifndef HONEYCORD_I420_ABTASTER_H_
#define HONEYCORD_I420_ABTASTER_H_

#include <cstddef>
#include <cstdint>
#include <vector>

namespace honeycord {

// Arbeitsspeicher, ueber Bilder wiederverwendet. Spannen und Kehrwerte haengen
// nur an der Geometrie (Quell- und Zielgroesse) und werden nur bei deren
// Wechsel neu gerechnet.
struct I420Abtaster {
  // Geometrie, fuer die die Tabellen unten gelten (-1 = noch keine).
  int qw = -1, qh = -1, zw = -1, zh = -1;
  // Spaltenspannen: Start und Breite je Zielspalte (Y bzw. Farbebene).
  std::vector<int> y_von, y_n, c_von, c_n;
  // Kehrwerte 2^32/(n*m), gerundet, Index n*m (0 unbenutzt).
  std::vector<uint64_t> y_kehr, c_kehr;
  // Spaltensummen einer Zielzeile ueber ihre Quellzeilen.
  std::vector<uint32_t> summe_y, summe_u, summe_v;
  // Farbanteile je Zielspalte der Farbebene (fuer B, G, R; schon * Koeffizient).
  std::vector<int32_t> anteil_b, anteil_g, anteil_r;
};

// Gilt der direkte Weg fuer diese Groessen? Beide Achsen deutlich (mehr als
// halb) verkleinert — dieselbe Grenze, ab der libyuv vom bilinearen auf den
// Box-Filter wechselt (ScaleFilterReduce).
inline bool I420DirektGeeignet(int qw, int qh, int zw, int zh) {
  return qw > 0 && qh > 0 && zw > 0 && zh > 0 && zw * 2 < qw && zh * 2 < qh;
}

namespace abtaster_intern {

// Start der Spanne fuer Zielindex i (0..ziel) — ganzzahlig, monoton, letzte
// Grenze = quelle. 64 bit gegen Ueberlauf.
inline int SpannenGrenze(int i, int quelle, int ziel) {
  return static_cast<int>(static_cast<int64_t>(i) * quelle / ziel);
}

inline void Spannen(int quelle, int ziel, std::vector<int>* von,
                    std::vector<int>* n, int* n_max) {
  von->resize(static_cast<size_t>(ziel));
  n->resize(static_cast<size_t>(ziel));
  *n_max = 1;
  for (int i = 0; i < ziel; ++i) {
    const int a = SpannenGrenze(i, quelle, ziel);
    int b = SpannenGrenze(i + 1, quelle, ziel);
    if (b <= a) b = a + 1;  // nie leer (bei ziel < quelle ohnehin nicht)
    (*von)[static_cast<size_t>(i)] = a;
    (*n)[static_cast<size_t>(i)] = b - a;
    if (b - a > *n_max) *n_max = b - a;
  }
}

inline void Kehrwerte(int nm_max, std::vector<uint64_t>* kehr) {
  kehr->resize(static_cast<size_t>(nm_max) + 1);
  (*kehr)[0] = 0;
  for (int nm = 1; nm <= nm_max; ++nm) {
    const uint64_t d = static_cast<uint64_t>(nm);
    (*kehr)[static_cast<size_t>(nm)] = ((1ULL << 32) + d / 2) / d;
  }
}

// Mittelwert = summe / nm, gerundet (summe <= 255 * nm).
inline uint32_t Mittel(uint32_t summe, uint64_t kehr) {
  return static_cast<uint32_t>((summe * kehr + (1ULL << 31)) >> 32);
}

// Y-Anteil wie libyuv: ((y * 0x0101 * YG) >> 16) + YB.
inline const int32_t* YTabelle() {
  static const std::vector<int32_t> tabelle = [] {
    std::vector<int32_t> t(256);
    for (uint32_t y = 0; y < 256; ++y) {
      t[y] = static_cast<int32_t>((y * 0x0101u * 18997u) >> 16) - 1160;
    }
    return t;
  }();
  return tabelle.data();
}

inline uint8_t Klemmen6(int32_t v) {
  v >>= 6;  // arithmetisch (MSVC/clang/gcc), wie libyuv
  return static_cast<uint8_t>(v < 0 ? 0 : (v > 255 ? 255 : v));
}

// Spaltensummen der Quellzeilen [z0, z1) einer Ebene (breite Bytes je Zeile).
inline void ZeilenSummieren(const uint8_t* ebene, int stride, int breite,
                            int z0, int z1, uint32_t* summe) {
  const uint8_t* zeile = ebene + static_cast<ptrdiff_t>(z0) * stride;
  for (int x = 0; x < breite; ++x) summe[x] = zeile[x];
  for (int z = z0 + 1; z < z1; ++z) {
    zeile = ebene + static_cast<ptrdiff_t>(z) * stride;
    for (int x = 0; x < breite; ++x) summe[x] += zeile[x];
  }
}

}  // namespace abtaster_intern

// Verkleinert ein I420-Bild (qw x qh, Farbebenen (qw+1)/2 x (qh+1)/2) auf
// zw x zh und schreibt RGBA (4 Byte je Pixel, Zeilenabstand zw * 4) nach
// `ziel` (mindestens zw * zh * 4 Byte). Rueckgabe false, wenn Zeiger fehlen,
// Zeilenabstaende zu klein sind oder die Groessen nicht passen (dann ist
// `ziel` unberuehrt) — oder im nie erwarteten Fall einer Spanne ausserhalb
// der Kehrwert-Tabelle (dann teilweise beschrieben). In beiden Faellen nimmt
// der Aufrufer den alten Weg, der den ganzen Puffer neu schreibt.
// Gelesen wird ausschliesslich innerhalb der Ebenen: Zeilen [0, qh) bzw.
// [0, (qh+1)/2), Spalten [0, qw) bzw. [0, (qw+1)/2).
inline bool I420NachRgbaVerkleinern(const uint8_t* y, int stride_y,
                                    const uint8_t* u, int stride_u,
                                    const uint8_t* v, int stride_v, int qw,
                                    int qh, uint8_t* ziel, int zw, int zh,
                                    I420Abtaster* a) {
  using namespace abtaster_intern;
  if (!y || !u || !v || !ziel || !a) return false;
  if (!I420DirektGeeignet(qw, qh, zw, zh)) return false;
  const int cw = (qw + 1) / 2, ch = (qh + 1) / 2;  // Farbebene (Quelle)
  const int czw = (zw + 1) / 2, czh = (zh + 1) / 2;  // Farbebene (Ziel)
  if (stride_y < qw || stride_u < cw || stride_v < cw) return false;

  if (a->qw != qw || a->qh != qh || a->zw != zw || a->zh != zh) {
    int ny = 1, nc = 1;
    Spannen(qw, zw, &a->y_von, &a->y_n, &ny);
    Spannen(cw, czw, &a->c_von, &a->c_n, &nc);
    // Zeilenspannen werden je Zeile gerechnet; ihr Hoechstwert hier:
    const int my = (qh + zh - 1) / zh + 1;
    const int mc = (ch + czh - 1) / czh + 1;
    Kehrwerte(ny * my, &a->y_kehr);
    Kehrwerte(nc * mc, &a->c_kehr);
    a->summe_y.resize(static_cast<size_t>(qw));
    a->summe_u.resize(static_cast<size_t>(cw));
    a->summe_v.resize(static_cast<size_t>(cw));
    a->anteil_b.resize(static_cast<size_t>(czw));
    a->anteil_g.resize(static_cast<size_t>(czw));
    a->anteil_r.resize(static_cast<size_t>(czw));
    a->qw = qw;
    a->qh = qh;
    a->zw = zw;
    a->zh = zh;
  }

  const int32_t* ytab = YTabelle();
  uint32_t* sy = a->summe_y.data();
  uint32_t* su = a->summe_u.data();
  uint32_t* sv = a->summe_v.data();
  int32_t* ab = a->anteil_b.data();
  int32_t* ag = a->anteil_g.data();
  int32_t* ar = a->anteil_r.data();
  const int* y_von = a->y_von.data();
  const int* y_n = a->y_n.data();
  const int* c_von = a->c_von.data();
  const int* c_n = a->c_n.data();
  const size_t y_kehr_max = a->y_kehr.size();
  const size_t c_kehr_max = a->c_kehr.size();

  int c_zeile = -1;  // zuletzt gerechnete Farbzeile (Ziel)
  for (int oy = 0; oy < zh; ++oy) {
    // Farbzeile oy/2: einmal je zwei Zielzeilen.
    const int cy = oy / 2;
    if (cy != c_zeile && cy < czh) {
      c_zeile = cy;
      const int z0 = SpannenGrenze(cy, ch, czh);
      int z1 = SpannenGrenze(cy + 1, ch, czh);
      if (z1 <= z0) z1 = z0 + 1;
      ZeilenSummieren(u, stride_u, cw, z0, z1, su);
      ZeilenSummieren(v, stride_v, cw, z0, z1, sv);
      const int m = z1 - z0;
      for (int cx = 0; cx < czw; ++cx) {
        const int x0 = c_von[cx], n = c_n[cx];
        uint32_t s_u = 0, s_v = 0;
        for (int k = 0; k < n; ++k) {
          s_u += su[x0 + k];
          s_v += sv[x0 + k];
        }
        const size_t nm = static_cast<size_t>(n) * static_cast<size_t>(m);
        if (nm >= c_kehr_max) return false;  // kann nicht vorkommen (Schutz)
        const uint64_t kehr = a->c_kehr[nm];
        const int32_t ui = static_cast<int32_t>(Mittel(s_u, kehr)) - 128;
        const int32_t vi = static_cast<int32_t>(Mittel(s_v, kehr)) - 128;
        ab[cx] = ui * 128;
        ag[cx] = ui * 25 + vi * 52;
        ar[cx] = vi * 102;
      }
    }
    const int z0 = SpannenGrenze(oy, qh, zh);
    int z1 = SpannenGrenze(oy + 1, qh, zh);
    if (z1 <= z0) z1 = z0 + 1;
    ZeilenSummieren(y, stride_y, qw, z0, z1, sy);
    const int m = z1 - z0;
    uint8_t* aus = ziel + static_cast<ptrdiff_t>(oy) * zw * 4;
    for (int ox = 0; ox < zw; ++ox) {
      const int x0 = y_von[ox], n = y_n[ox];
      uint32_t s = 0;
      for (int k = 0; k < n; ++k) s += sy[x0 + k];
      const size_t nm = static_cast<size_t>(n) * static_cast<size_t>(m);
      if (nm >= y_kehr_max) return false;  // kann nicht vorkommen (Schutz)
      const int32_t y1 = ytab[Mittel(s, a->y_kehr[nm])];
      const int cx = ox / 2;
      aus[0] = Klemmen6(y1 + ar[cx]);  // R
      aus[1] = Klemmen6(y1 - ag[cx]);  // G
      aus[2] = Klemmen6(y1 + ab[cx]);  // B
      aus[3] = 255;                    // A
      aus += 4;
    }
  }
  return true;
}

}  // namespace honeycord

#endif  // HONEYCORD_I420_ABTASTER_H_
