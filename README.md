# deskControl – Desktops umschalten unter Wayland (passend zu mwc)

Eine C-Datei, nur libwayland, xkbcommon, Cairo und Pango, kein Toolkit. Motif-Look wie
[pulseControl](https://github.com/reygnn/pulseControl) und [mfm](https://github.com/reygnn/mfm),
mit denselben Farben wie [mwc](https://github.com/reygnn/mwc): `MWC_INACTIVE` /
`MWC_ACTIVE` aus der Umgebung, sonst `Mwm*background` / `Mwm*activeBackground` aus
`~/.config/mwc/mwmrc`.

deskControl zeigt einen Knopf pro Desktop, in ihrer Reihenfolge, zwei pro Reihe: 1 2
über 3 4. Der aktuelle ist eingedrückt, in der Aktiv-Farbe. Ein Desktop, auf dem ein
Fenster Aufmerksamkeit will, ist bernsteinfarben markiert, bis man hinwechselt oder das
Fenster zugeht. Schaltet ein anderes Programm um, zieht deskControl nach.

Es spricht `ext-workspace-v1`, läuft also mit jedem Compositor, der das anbietet; mwc
tut es ab der Version mit virtuellen Desktops.

## Bauen

    sudo apt install build-essential pkg-config libwayland-dev wayland-protocols \
        libxkbcommon-dev libcairo2-dev libpango1.0-dev
    make
    sudo make install        # -> /usr/local/bin/deskControl

`wayland-protocols` braucht mindestens 1.40 (Ubuntu 26.04 hat 1.47).

## In mwc

Ein normales Fenster gehört in mwc zu einem Desktop und wäre nach dem ersten Wechsel
weg. In die `~/.config/mwc/mwmrc`:

```
Mwm*deskControl*allDesktops:   true
Mwm*deskControl*geometry:      -0+0
```

Die erste Zeile legt es auf alle Desktops, die zweite in die Ecke oben rechts.

## Bedienung

| Eingabe | Aktion |
|---|---|
| Klick auf einen Knopf | zu diesem Desktop |
| Mausrad runter / hoch | zum nächsten / vorigen |
| 1 bis 9 | zu diesem Desktop |
| → ↓ / ← ↑ | zum nächsten / vorigen, an den Enden bleibt es stehen |
| Pos1 / Ende | zum ersten / letzten |
| Esc / q / Ctrl+Q / Ctrl+W | beenden |

Ein Knopf zählt beim Loslassen: wer gedrückt vom Knopf wegzieht, schaltet nicht um. Mit
Alt, Super oder Ctrl gedrückt wirkt keine Taste, außer Ctrl+Q und Ctrl+W. Tasten
wiederholen sich nicht. Das Fenster ist so groß wie seine Knöpfe.

Es hat nur einen dünnen Motif-Rahmen, den es selbst zeichnet: keine Titelleiste, keine
Fensterknöpfe, keine Griffe zum Ziehen. Hat es die Tastatur, ist der Rahmen in der
Aktiv-Farbe, wie bei mwcs Fenstern. Wo es steht, legt `geometry` fest.

`deskControl -v` zeigt die Version.

## Umgebung

| Variable | Standard | |
|---|---|---|
| `DC_FONT` | `Sans 10` | Pango-Name, z. B. `Sans Bold 11` |
| `MWC_INACTIVE`, `MWC_ACTIVE` | `#aeb2c3`, `#7387ad` | Hintergrund, aktueller Desktop |

## Tests

    make test

fährt deskControl unter einem headless laufenden mwc mit simulierter Maus und Tastatur
und prüft mwcs Zustandsdatei und Screenshots. Es braucht einen gebauten mwc samt
`build/vinput`, `build/pngtool`, `build/workspacec` und `build/xclient` in `MWC_DIR`
(sonst `../mwc`), dazu `grim` und `Xwayland`. Am einfachsten in der Entwicklungsbox
von mwc.

## Lizenz

MIT, siehe [LICENSE](LICENSE).
