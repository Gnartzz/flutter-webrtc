// Linux: Flutter-Textur, die GPU-Bilder ohne Umweg zeigt (10.11.2026).
// Siehe hc_gl_textur.cc und hc_gpu_textur.h.
#pragma once

#include <flutter_linux/flutter_linux.h>

#include "flutter/texture_registrar.h"

// Soll eine neue Textur den GL-Weg nehmen? (Wayland und nicht abgeschaltet)
bool hc_gl_textur_moeglich();

// Neue GL-Textur für [variante] (muss ein PixelBufferTexture sein).
FlTexture* hc_gl_textur_neu(flutter::TextureVariant* variante);
