# FeeFS

Ein selbst entworfenes Dateisystem in C, das komplett in einem Byte-Array im Speicher lebt (Disk-Simulation).

## Hintergrund

Das Projekt ist größtenteils entstanden, um **Neovim (nvim)** zu lernen. Dabei wurde auch **KI** als Hilfsmittel verwendet. Das Konzept und die Idee stammen jedoch von mir.

## Aufbau

| Datei | Inhalt |
|-------|--------|
| `fs.h` / `fs.c` | Das eigentliche Dateisystem: formatieren, mounten, Ordner und Dateien anlegen, laden, ändern, löschen. Pfade sind relativ zu Root. |
| `rfs.h` / `rfs.c` | Schicht darüber mit Arbeitsverzeichnis: relative Pfade, `.` und `..`, Navigation sowie Dateien öffnen, lesen, schreiben und schließen. |
| `fsm.c` | Kleines Beispielprogramm, das RFS verwendet. |

## Benutzung

```bash
gcc fsm.c rfs.c fs.c -o fsm
./fsm
```

```c
FS_formatDrive(disk, size);
FS_mountDrive(disk);

RFS_createFolder("docs");
RFS_changeDir("docs");

TRFS_FILE* f;
RFS_openFile("hallo.txt", RFS_MODE_CREATE, &f);
RFS_writeFile(f, 0, "Hallo RFS!", 10);
RFS_closeFile(f);
```

Die genaue Beschreibung aller Funktionen und Fehlercodes steht als Kommentare in `fs.h` und `rfs.h`.
