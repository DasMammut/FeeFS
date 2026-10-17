#ifndef FS_H
#define FS_H

#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

/*
 * Ergebniscode aller oeffentlichen FS-Funktionen.
 * Funktionen geben FSE_SUCCESS bei Erfolg zurueck; die jeweils moeglichen
 * weiteren Werte sind in den Kommentaren an der Funktion aufgefuehrt.
 */
typedef enum EFS_ERROR{
    FSE_SUCCESS = 0,          /* Operation erfolgreich. */
    FSE_NOT_FORMATED,         /* FS_mountDrive: kein gueltig formatiertes Laufwerk. */
    FSE_NOT_MOUNTED,          /* FS-Operation ohne aktuell gemountetes Laufwerk. */
    FSE_BLOCKS_CORRUPTED,     /* Inkonsistente oder ungueltige Block-/Ordner-Metadaten. */
    FSE_TO_FEW_ALLOCATED,     /* FS_mountDrive: Blockkette deckt nicht die ganze Disk ab. */
    FSE_TO_MUCH_ALLOCATED,    /* FS_mountDrive: Blockkette reicht ueber das Disk-Ende hinaus. */
    FSE_PATH_NOT_FOUND,       /* Pfad oder angefordertes Element nicht gefunden. */
    FSE_PATH_TOO_LONG,        /* Pfad hat mehr Komponenten als unterstuetzt werden. */
    FSE_NO_SPACE_AVALAIBLE,   /* Nicht genug freie Blockkapazitaet fuer die Operation. */
    FSE_INVALID_ARGUMENT,     /* NULL-, Groessen- oder Namensargument ungueltig. */
    FSE_MEMORY_ERROR,         /* Speicherreservierung fuer interne/ausgegebene Daten fehlgeschlagen. */
    FSE_DISK_TOO_SMALL,       /* FS_formatDrive: Disk kleiner als die Mindestgroesse. */
    FSE_NAME_TOO_LONG,        /* Name passt nicht in die verfuegbare Record-Kapazitaet. */
    FSE_ALREADY_EXISTS,       /* Gleichnamiger Eintrag des vom Aufruf geprueften Typs existiert. */
    FSE_INVAILD_ITEM_ID,      /* ID ist kein gueltiger sichtbarer Eintrag im Ordner. */
} TFS_ERROR;

/*
 * Dateiargument und Dateiergebnis.
 *
 * name ist ein gueltiger, nullterminierter C-String. size ist die Anzahl
 * Nutzdatenbytes; Daten duerfen binaer sein und muessen nicht nullterminiert
 * sein. Bei size == 0 darf data NULL sein; bei size > 0 muss data auf
 * mindestens size les-/schreibbare Bytes zeigen.
 *
 * FS_createFile und FS_editFile lesen diese Felder nur waehrend des Aufrufs:
 * der Aufrufer behaelt die Verantwortung fuer name und data; sie werden weder
 * uebernommen noch freigegeben.
 *
 * Bei erfolgreichem FS_loadFile sind name und data separat allokiert und
 * gehoeren dem Aufrufer. Beide muessen mit free() freigegeben werden. Auch bei
 * einer leeren Datei ist data allokiert und muss freigegeben werden. Vor einem
 * erneuten Laden in dasselbe Struct muessen zuvor erhaltene Puffer freigegeben
 * werden.
 */
typedef struct SFS_FILE {
    char* name;
    uint64_t size;
    uint8_t* data;
} TFS_FILE;

/*
 * Initialisiert die Disk mit einem leeren Dateisystem.
 * ACHTUNG: Der Inhalt von disk wird ueberschrieben. Die Disk muss mindestens
 * 1024 Bytes gross sein und bis zum FS_mountDrive-Aufruf gueltig bleiben.
 *
 * Fehler: FSE_INVALID_ARGUMENT (disk == NULL), FSE_DISK_TOO_SMALL,
 * FSE_NAME_TOO_LONG oder FSE_BLOCKS_CORRUPTED bei unerwarteten internen
 * Metadaten-/Kapazitaetsproblemen.
 */
TFS_ERROR FS_formatDrive(uint8_t* disk, uint64_t size);

/*
 * Mountet ein zuvor formatiertes Disk-Bytearray. Der Aufrufer behaelt den
 * Speicher und muss ihn waehrend der gesamten Nutzung des Dateisystems
 * gueltig halten. Der gespeicherte Disk-Inhalt wird auf Blockkonsistenz
 * geprueft.
 *
 * Fehler: FSE_INVALID_ARGUMENT, FSE_NOT_FORMATED, FSE_BLOCKS_CORRUPTED,
 * FSE_TO_FEW_ALLOCATED oder FSE_TO_MUCH_ALLOCATED.
 */
TFS_ERROR FS_mountDrive(uint8_t* disk);

/*
 * Pfade sind slash-getrennt und relativ zum Root-Ordner; "" bezeichnet Root.
 * Fuehrende/mehrfache/trailing Slashes werden beim Zerlegen ignoriert.
 * Namen einzelner Elemente duerfen kein '/' enthalten.
 */

/*
 * Gibt die Anzahl sichtbarer Eintraege im Ordner path zurueck. Der interne
 * Parent-/Root-Datensatz eines Unterordners wird nicht mitgezaehlt.
 *
 * Fehler: FSE_INVALID_ARGUMENT, FSE_NOT_MOUNTED, FSE_PATH_NOT_FOUND,
 * FSE_PATH_TOO_LONG, FSE_BLOCKS_CORRUPTED oder FSE_MEMORY_ERROR.
 */
TFS_ERROR FS_getItemCount(char* path, uint64_t* count);

/*
 * Liefert den Namen des sichtbaren Eintrags mit ID id in path. IDs sind
 * nullbasiert und folgen der Reihenfolge der Eintraege im Ordner; der interne
 * Parent-Datensatz wird ausgelassen.
 *
 * Bei Erfolg wird *name auf einen mit malloc() reservierten, nullterminierten
 * Namen gesetzt. Der Aufrufer muss ihn mit free() freigeben. Vor einem erneuten
 * Aufruf in dasselbe *name muss ein zuvor ausgegebener Puffer freigegeben werden.
 *
 * Fehler: FSE_INVALID_ARGUMENT, FSE_NOT_MOUNTED, FSE_PATH_NOT_FOUND,
 * FSE_PATH_TOO_LONG, FSE_INVAILD_ITEM_ID, FSE_BLOCKS_CORRUPTED oder
 * FSE_MEMORY_ERROR.
 */
TFS_ERROR FS_nameByID(char* path, uint64_t id, char** name);

/*
 * Schreibt in *isFolder, ob der sichtbare Eintrag id in path ein Ordner ist.
 * IDs haben dieselbe nullbasierte Reihenfolge wie bei FS_nameByID.
 *
 * Fehler: FSE_INVALID_ARGUMENT, FSE_NOT_MOUNTED, FSE_PATH_NOT_FOUND,
 * FSE_PATH_TOO_LONG, FSE_INVAILD_ITEM_ID, FSE_BLOCKS_CORRUPTED oder
 * FSE_MEMORY_ERROR.
 */
TFS_ERROR FS_isItemFolderByID(char* path, uint64_t id, bool* isFolder);

/*
 * Erstellt den Unterordner name in path. name muss nichtleer sein und darf
 * keinen '/' enthalten. Ein gleichnamiger Ordner ist nicht erlaubt; eine
 * gleichnamige Datei darf daneben existieren.
 *
 * Fehler: FSE_INVALID_ARGUMENT, FSE_NOT_MOUNTED, FSE_PATH_NOT_FOUND,
 * FSE_PATH_TOO_LONG, FSE_ALREADY_EXISTS, FSE_NO_SPACE_AVALAIBLE,
 * FSE_NAME_TOO_LONG, FSE_BLOCKS_CORRUPTED oder FSE_MEMORY_ERROR.
 */
TFS_ERROR FS_createFolder(char* path, char* name);

/*
 * Loescht den Ordner name in path einschliesslich aller Dateien und
 * Unterordner (rekursiv). Ein gleichnamiger Datei-Eintrag bleibt erhalten.
 * Der Ordner muss nicht leer sein.
 *
 * Fehler: FSE_INVALID_ARGUMENT, FSE_NOT_MOUNTED, FSE_PATH_NOT_FOUND,
 * FSE_PATH_TOO_LONG, FSE_BLOCKS_CORRUPTED oder FSE_MEMORY_ERROR.
 */
TFS_ERROR FS_deleteFolder(char* path, char* name);

/*
 * Laedt die Datei name aus path. Bei Erfolg werden file->name, file->size
 * und file->data befuellt. Die Daten enthalten genau size Bytes; der Name ist
 * nicht Teil der Nutzdaten. Beide ausgegebenen Puffer gehoeren dem Aufrufer
 * und muessen separat mit free() freigegeben werden.
 *
 * Fehler: FSE_INVALID_ARGUMENT, FSE_NOT_MOUNTED, FSE_PATH_NOT_FOUND,
 * FSE_PATH_TOO_LONG, FSE_BLOCKS_CORRUPTED oder FSE_MEMORY_ERROR.
 */
TFS_ERROR FS_loadFile(char* path, char* name, TFS_FILE* file);

/*
 * Erstellt eine Datei anhand von file->name, file->size und file->data in
 * path. name muss nichtleer und slash-frei sein; data muss bei positiver
 * Groesse gueltig sein. Ein gleichnamiger Datei-Eintrag ist nicht erlaubt,
 * ein gleichnamiger Ordner dagegen schon. Das uebergebene Struct und seine
 * Puffer bleiben im Besitz des Aufrufers.
 *
 * Fehler: FSE_INVALID_ARGUMENT, FSE_NOT_MOUNTED, FSE_PATH_NOT_FOUND,
 * FSE_PATH_TOO_LONG, FSE_ALREADY_EXISTS, FSE_NO_SPACE_AVALAIBLE,
 * FSE_NAME_TOO_LONG, FSE_BLOCKS_CORRUPTED oder FSE_MEMORY_ERROR.
 */
TFS_ERROR FS_createFile(char* path, TFS_FILE* file);

/*
 * Ersetzt die Nutzdaten der bestehenden Datei file->name in path. Der Name
 * dient zum Auffinden der Datei und kann durch diesen Aufruf nicht geaendert
 * werden. Die Datei kann dabei wachsen oder kleiner werden; size bezeichnet
 * die neue Nutzdatengroesse. Das uebergebene Struct und seine Puffer bleiben
 * im Besitz des Aufrufers.
 *
 * Fehler: FSE_INVALID_ARGUMENT, FSE_NOT_MOUNTED, FSE_PATH_NOT_FOUND,
 * FSE_PATH_TOO_LONG, FSE_NO_SPACE_AVALAIBLE, FSE_BLOCKS_CORRUPTED oder
 * FSE_MEMORY_ERROR.
 */
TFS_ERROR FS_editFile(char* path, TFS_FILE* file);

/*
 * Loescht die Datei name aus path und gibt ihre Bloecke frei. Ein gleichnamiger
 * Ordner wird nicht geloescht.
 *
 * Fehler: FSE_INVALID_ARGUMENT, FSE_NOT_MOUNTED, FSE_PATH_NOT_FOUND,
 * FSE_PATH_TOO_LONG, FSE_BLOCKS_CORRUPTED oder FSE_MEMORY_ERROR.
 */
TFS_ERROR FS_deleteFile(char* path, char* name);

#endif
