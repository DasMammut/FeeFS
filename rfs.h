#ifndef RFS_H
#define RFS_H

#include <stdint.h>
#include <stdbool.h>
#include "fs.h"

/*
 * Relative Schicht ueber fs.h. Es gibt ein aktuelles Arbeitsverzeichnis
 * (zu Beginn Root); alle Pfade sind relativ dazu. Unterstuetzt werden
 * Komponenten wie "ordner/datei", "." und "..". Ein fuehrender Slash
 * bezeichnet den Root-Ordner und ist damit ein absoluter Pfad. ".." ueber
 * Root hinaus ergibt FSE_PATH_NOT_FOUND. Alle Funktionen geben TFS_ERROR
 * aus fs.h zurueck; ein gemountetes Laufwerk (FS_mountDrive) wird
 * vorausgesetzt.
 */

/*
 * Oeffnungsmodus fuer RFS_openFile.
 */
typedef enum ERFS_MODE{
    RFS_MODE_READ = 0,    /* Datei muss existieren; nur lesen. */
    RFS_MODE_WRITE,       /* Datei muss existieren; lesen und schreiben. */
    RFS_MODE_CREATE,      /* Datei wird bei Bedarf leer angelegt; lesen und schreiben. */
} TRFS_MODE;

/*
 * Handle einer geoeffneten Datei. Der Inhalt liegt waehrend des Oeffnens im
 * Speicher; Aenderungen werden mit RFS_flushFile oder RFS_closeFile in das
 * Dateisystem geschrieben. Die Felder sind nur intern zu verwenden; Zugriff
 * erfolgt ueber die RFS_*-Funktionen.
 */
typedef struct SRFS_FILE {
    char* dir;
    TFS_FILE file;
    uint64_t capacity;
    bool writable;
    bool dirty;
} TRFS_FILE;

/*
 * Setzt das Arbeitsverzeichnis auf Root zurueck. Optional; ohne Aufruf
 * beginnt das Arbeitsverzeichnis ebenfalls in Root.
 */
void RFS_reset(void);

/*
 * Wechselt das Arbeitsverzeichnis nach path (relativ oder mit fuehrendem
 * Slash absolut). Das Ziel muss ein existierender Ordner sein; bei einem
 * Fehler bleibt das Arbeitsverzeichnis unveraendert.
 *
 * Fehler: FSE_INVALID_ARGUMENT, FSE_NOT_MOUNTED, FSE_PATH_NOT_FOUND,
 * FSE_PATH_TOO_LONG, FSE_BLOCKS_CORRUPTED, FSE_MEMORY_ERROR.
 */
TFS_ERROR RFS_changeDir(char* path);

/*
 * Liefert den absoluten Pfad des Arbeitsverzeichnisses ("/" fuer Root).
 * Bei Erfolg ist *path mit malloc() reserviert und muss vom Aufrufer mit
 * free() freigegeben werden.
 *
 * Fehler: FSE_INVALID_ARGUMENT, FSE_MEMORY_ERROR.
 */
TFS_ERROR RFS_getWorkingDir(char** path);

/*
 * Wie FS_getItemCount, aber path ist relativ zum Arbeitsverzeichnis;
 * "" oder "." bezeichnet das Arbeitsverzeichnis selbst.
 */
TFS_ERROR RFS_getItemCount(char* path, uint64_t* count);

/*
 * Wie FS_nameByID, aber path ist relativ zum Arbeitsverzeichnis. *name
 * gehoert dem Aufrufer und muss mit free() freigegeben werden.
 */
TFS_ERROR RFS_nameByID(char* path, uint64_t id, char** name);

/*
 * Wie FS_isItemFolderByID, aber path ist relativ zum Arbeitsverzeichnis.
 */
TFS_ERROR RFS_isItemFolderByID(char* path, uint64_t id, bool* isFolder);

/*
 * Erstellt den Ordner, auf den path zeigt. Der letzte Pfadbestandteil ist der
 * neue Name, alle davor muessen existieren (z. B. "a/b" legt b in a an).
 *
 * Fehler: FSE_INVALID_ARGUMENT, FSE_NOT_MOUNTED, FSE_PATH_NOT_FOUND,
 * FSE_PATH_TOO_LONG, FSE_ALREADY_EXISTS, FSE_NO_SPACE_AVALAIBLE,
 * FSE_NAME_TOO_LONG, FSE_BLOCKS_CORRUPTED, FSE_MEMORY_ERROR.
 */
TFS_ERROR RFS_createFolder(char* path);

/*
 * Loescht den Ordner, auf den path zeigt, rekursiv. Liegt das
 * Arbeitsverzeichnis im geloeschten Ordner, wird es auf dessen
 * uebergeordneten Ordner gesetzt. Root kann nicht geloescht werden
 * (FSE_INVALID_ARGUMENT).
 *
 * Fehler: FSE_INVALID_ARGUMENT, FSE_NOT_MOUNTED, FSE_PATH_NOT_FOUND,
 * FSE_PATH_TOO_LONG, FSE_BLOCKS_CORRUPTED, FSE_MEMORY_ERROR.
 */
TFS_ERROR RFS_deleteFolder(char* path);

/*
 * Loescht die Datei, auf die path zeigt. Geoeffnete Handles auf diese Datei
 * werden dadurch nicht ungueltig, ein spaeteres Flush schlaegt aber fehl.
 *
 * Fehler: FSE_INVALID_ARGUMENT, FSE_NOT_MOUNTED, FSE_PATH_NOT_FOUND,
 * FSE_PATH_TOO_LONG, FSE_BLOCKS_CORRUPTED, FSE_MEMORY_ERROR.
 */
TFS_ERROR RFS_deleteFile(char* path);

/*
 * Oeffnet die Datei, auf die path zeigt, und laedt ihren Inhalt. Bei Erfolg ist
 * *file ein mit malloc() reserviertes Handle, das mit RFS_closeFile
 * freigegeben werden muss. Bei RFS_MODE_CREATE wird eine fehlende Datei leer
 * angelegt.
 *
 * Fehler: FSE_INVALID_ARGUMENT, FSE_NOT_MOUNTED, FSE_PATH_NOT_FOUND,
 * FSE_PATH_TOO_LONG, FSE_NO_SPACE_AVALAIBLE, FSE_NAME_TOO_LONG,
 * FSE_BLOCKS_CORRUPTED, FSE_MEMORY_ERROR.
 */
TFS_ERROR RFS_openFile(char* path, TRFS_MODE mode, TRFS_FILE** file);

/*
 * Liest bis zu len Bytes ab offset in buffer. In *read steht die tatsaechlich
 * gelesene Anzahl (0 bei offset am oder hinter dem Dateiende). read darf NULL
 * sein.
 *
 * Fehler: FSE_INVALID_ARGUMENT.
 */
TFS_ERROR RFS_readFile(TRFS_FILE* file, uint64_t offset, void* buffer, uint64_t len, uint64_t* read);

/*
 * Schreibt len Bytes ab offset in die geoeffnete Datei. Die Datei waechst bei
 * Bedarf; eine entstehende Luecke wird mit 0 gefuellt. Die Aenderung wird erst
 * durch RFS_flushFile bzw. RFS_closeFile persistent.
 *
 * Fehler: FSE_INVALID_ARGUMENT (auch bei Handle im Lesemodus),
 * FSE_MEMORY_ERROR.
 */
TFS_ERROR RFS_writeFile(TRFS_FILE* file, uint64_t offset, const void* buffer, uint64_t len);

/*
 * Setzt die Dateigroesse auf size; neue Bytes sind 0. Erst RFS_flushFile bzw.
 * RFS_closeFile macht die Aenderung persistent.
 *
 * Fehler: FSE_INVALID_ARGUMENT (auch bei Handle im Lesemodus),
 * FSE_MEMORY_ERROR.
 */
TFS_ERROR RFS_resizeFile(TRFS_FILE* file, uint64_t size);

/*
 * Gibt die aktuelle Dateigroesse in *size zurueck.
 *
 * Fehler: FSE_INVALID_ARGUMENT.
 */
TFS_ERROR RFS_fileSize(TRFS_FILE* file, uint64_t* size);

/*
 * Schreibt ausstehende Aenderungen mittels FS_editFile in das Dateisystem.
 * Ohne Aenderungen passiert nichts.
 *
 * Fehler: FSE_INVALID_ARGUMENT, FSE_NOT_MOUNTED, FSE_PATH_NOT_FOUND,
 * FSE_NO_SPACE_AVALAIBLE, FSE_BLOCKS_CORRUPTED, FSE_MEMORY_ERROR.
 */
TFS_ERROR RFS_flushFile(TRFS_FILE* file);

/*
 * Schreibt ausstehende Aenderungen und gibt das Handle in jedem Fall frei.
 * Nach dem Aufruf darf file nicht mehr verwendet werden. Der Rueckgabewert
 * ist das Ergebnis des Flush; schlaegt er fehl, gehen die Aenderungen
 * verloren. NULL ist erlaubt.
 */
TFS_ERROR RFS_closeFile(TRFS_FILE* file);

#endif // RFS_H
