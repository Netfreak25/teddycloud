# RUID-Bilder und Inhaltscover

Ein Custom-Bild gehört ausschließlich zur RUID, nicht zum Modell oder zur
Library-Collection. Die Anzeige verändert keine Source, Audiodateien, NoCloud-
oder Freshness-Zustände.

| Kontext | Bildpriorität |
| --- | --- |
| Figur, Karte, Boxbild, Export | Custom-Bild, Modellbild, Platzhalter |
| Inhalt mit konkreter RUID | bekanntes Inhaltscover, Custom-Bild, Modellbild, Platzhalter |
| Library ohne RUID-Kontext | Collection-Cover, Platzhalter |

`img_unknown.png` gilt auch in absoluten URLs mit Queryparametern als Platzhalter.
Die WebUI verwendet die gemeinsame Auflösung in `imagePathUtils.ts` und löst
relative Bilder gegen die konfigurierte API-Basis auf. Native Player-Einträge
können einen expliziten `tonieRuid`-Anzeigekontext tragen; ihre Collection-ID und
Audioquellen bleiben unverändert. Ein beliebiger anderer Tonie mit derselben
Collection ist keine Bildquelle. Bei nativen Sources liefert `getTagInfo` das
bereits vorhandene `sourceInfo`-Feld auch für das aufgelöste Collection-Cover.

## Upload

Der Tonie-Editor dekodiert neue PNG/JPEG/WebP/GIF-Dateien bis 5 MiB und passt sie
proportional in ein transparentes 512×512-PNG ein. Kleine Bilder werden vergrößert,
Animationen auf das erste/default Einzelbild reduziert. Vorschau und Upload
verwenden dieselbe PNG-Datei. Fehler, Abbruch oder eine inzwischen verworfene
Auswahl lösen keinen Upload aus. Bestehende Bilder und direkte API-Uploads werden
nicht konvertiert. Serverseitige Signaturprüfung, Größenlimit und atomarer
Austausch der einzigen aktiven RUID-Bilddatei bleiben erhalten.

## Lokale MQTT/Home-Assistant-Integration

Das bestehende `ContentPicture`-Topic und die HASS-Bildentität bleiben erhalten.
TB1 publiziert beim RTNL-Audio-ID-Ereignis; TB2 bei einem Wechsel der aktuellen
Playback-RUID oder Content-Version. Kapitel- und Positionsmeldungen lösen keine
erneute Bildauflösung aus. Stop verwendet weiterhin `img_empty.png`.

`tonie_picture.c` liest nur vorhandene Content-Zuordnungen, Katalogdaten und Bilder;
es erzeugt oder repariert keinen Content. TB2 verwendet für Custom-Bilder nur
die gültige RUID aus der aktuellen `playback/state.tonie`-Meldung, niemals eine
von vorheriger Wiedergabe übrig gebliebene RUID. Relative Bildpfade werden mit
`core.host_url` zu absoluten URLs. Die Adresse muss vom MQTT/HASS-Empfänger
erreichbar sein.

Upload, Austausch und Entfernen aktualisieren die Bildausgabe aktuell betroffener
Boxen. Custom-Bild-URLs enthalten `?v=<SHA256 der Bildbytes>`, sodass ein Austausch
auch beim Empfänger als neues Bild erkennbar ist. Echte Inhaltscover behalten
Vorrang. Dies betrifft nur den lokalen Integrationsclient (`mqtt.enabled`), nicht
ICI-/TONIES-Weiterleitung, Filter oder Steuerbefehle.

## Fokussierte Verifikation

- `python tests/test_tonie_user_metadata_contract.py`
- `node tests/test_tonie_image_selection.mjs`
- Optional `--browser` mit vorhandenem Playwright/Chromium über
  `TONIE_IMAGE_PLAYWRIGHT_MODULE` und `TONIE_IMAGE_BROWSER`.
- Linux/WSL: `sh tests/run_tonie_picture_checks.sh` kompiliert die betroffenen
  C-Pfade und prüft den echten Playback-/Publisher-Code mit abgefangener
  MQTT-Ausgabe und kontrollierter Bildauflösung. Kein laufender Broker nötig.

Eine reale HASS-/Box-Installation wird dadurch nicht getestet. Dort nach Aktivierung
Custom-Bild setzen/ersetzen/entfernen, Inhaltscover mit Vorrang und Stop beobachten.
