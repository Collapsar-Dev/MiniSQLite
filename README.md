# MiniSQLite

A tiny educational SQL-like database written in C. Runs in the terminal,
stores everything in `database.json`. No SQLite or other database libraries;
the JSON reader/writer is implemented by hand in `main.c`.

## Build & run

Requirements: `gcc` (or any C99 compiler) and `make`, Linux/macOS.

```bash
make            # builds ./minidb
./minidb        # uses ./database.json (created on first launch)
./minidb my.json   # optional: use another database file
```

Without make: `gcc -std=c99 -Wall -O2 -o minidb main.c`

## Commands

Service commands:

| Command           | Description                       |
|-------------------|-----------------------------------|
| `.help`           | Show supported commands/examples  |
| `.tables`         | List tables                       |
| `.schema [table]` | Show table structure              |
| `.exit`           | Exit (also `.quit` or Ctrl+D)     |

SQL (keywords are case-insensitive):

```sql
CREATE TABLE students (id INTEGER, name TEXT, age INTEGER);
INSERT INTO students VALUES (1, 'Ani', 20);
SELECT * FROM students;
SELECT * FROM students WHERE id = 1;
SELECT name, age FROM students WHERE name = 'Ani';
UPDATE students SET age = 21 WHERE id = 1;
DELETE FROM students WHERE id = 2;
DROP TABLE students;
```

- Types: `INTEGER` (whole number, may be negative) and `TEXT` (in single quotes).
- `WHERE` supports one equality condition: `column = value`. All matching rows are affected.
- `UPDATE` / `DELETE` print how many rows were changed; without `WHERE` they affect all rows.

## Demo scenario

```
minidb> CREATE TABLE students (id INTEGER, name TEXT, age INTEGER);
minidb> INSERT INTO students VALUES (1, 'Ani', 20);
minidb> INSERT INTO students VALUES (2, 'Aram', 22);
minidb> INSERT INTO students VALUES (3, 'Narek', 19);
minidb> SELECT * FROM students;
minidb> SELECT * FROM students WHERE id = 1;
minidb> UPDATE students SET age = 21 WHERE id = 1;
minidb> DELETE FROM students WHERE id = 2;
minidb> .exit
$ ./minidb
minidb> SELECT * FROM students;      -- data is still there
minidb> SELEC * FROM students;       -- error, program keeps running
```

## How it works

- **Parsing a command** – the line is split into tokens (`tokenize`): words,
  numbers, `'text'`, and symbols `( ) , ; = *`. Then a small recursive-descent
  parser (`exec_create`, `exec_insert`, `exec_select`, `exec_update`,
  `exec_delete`) checks the tokens one by one against the expected grammar.
  The whole command is validated (table, columns, value count, types)
  **before** anything is changed, so a wrong command never modifies data.
- **Where the table structure lives** – in memory: `Database` → array of `Table`;
  each `Table` has a name, an array of `Column` (name + type) and an array of
  rows (each row is an array of `Value`). On disk: `database.json`, with
  `"columns"` (structure) and `"rows"` (data) for every table.
- **Finding rows to change** – `WHERE col = value` is parsed into a `Where`
  struct (column index + value). `UPDATE`, `DELETE` and `SELECT` scan every row
  and compare that column (`row_matches`).
- **Saving** – after every successful change the whole database is written to
  `database.json.tmp` and then renamed over `database.json` (atomic replace).
  On start the file is parsed and the tables are restored. If the file is
  damaged, the program stops with a message instead of overwriting it.

## Limitations

- One command per line, must end with `;`.
- Table/column names: Latin letters, digits and `_` (max 64 chars); names are case-insensitive.
- No quote escaping inside text values (`'` can't appear inside text).
- Only one equality condition in `WHERE` (no `AND/OR`, `<`, `>`).
- No `NULL`, `JOIN`, `ORDER BY`, indexes, transactions, auto-increment or uniqueness checks.
- `INSERT` must provide values for all columns in table order.
- Single user; the whole file is rewritten on each change (fine for small data).
