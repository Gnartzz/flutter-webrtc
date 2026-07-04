// HoneyCord — Windows-Audio-Ducking-Opt-out.
//
// Problem (Tester 2026-07-03): Sobald der Client das Mikrofon oeffnet, senkt
// Windows automatisch die Lautstaerke ALLER anderen Audio-Apps ab
// („Kommunikations-Ducking"). Ursache: libwebrtcs ADM waehlt Mikro/Ausgabe ueber
// die eCommunications-Rolle (Default = kDefaultCommunicationDevice) → Windows
// haelt die App fuer einen laufenden Anruf und duckt andere Streams um -80%.
//
// Fix (chirurgisch, in UNSEREM Code statt stock-webrtc-Patch): webrtc initiali-
// siert den Mikro-Client mit Session-GUID NULL = Prozess-Standard-Session auf
// dem Kommunikations-Aufnahmegeraet. Wir sprechen genau diese Session an
// (IAudioSessionManager2::GetAudioSessionControl(NULL)) und rufen
// IAudioSessionControl2::SetDuckingPreference(TRUE) → die Session nimmt nicht
// mehr am Ducking teil, andere Apps bleiben laut. MUSS vor IAudioClient::Start
// gesetzt sein → daher beim Plugin-Init (App-Start, lange vor Voice-Join).
#ifndef HONEYCORD_AUDIO_DUCKING_H_
#define HONEYCORD_AUDIO_DUCKING_H_

namespace honeycord {

// Einmal pro Prozess: Kommunikations-Aufnahme- UND -Wiedergabe-Session vom
// Windows-Ducking abmelden. Idempotent, schlaegt bei Fehlern still fehl
// (Audio funktioniert dann normal weiter, nur ohne Opt-out).
void OptOutCommunicationDuckingOnce();

}  // namespace honeycord

#endif  // HONEYCORD_AUDIO_DUCKING_H_
