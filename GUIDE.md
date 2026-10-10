# RajSQL: the simple guide

This page explains RajSQL in plain language: what it is, why it exists, how to use it, and what every part does. No database experience needed.

## 1. What is RajSQL?

RajSQL is a small **database engine**. An engine is the program that sits under SQL. When you type something like:

```sql
SELECT name FROM students WHERE gpa > 3.5;
```

somebody has to actually read that text, find the right rows, and send them back. Real databases like MySQL or PostgreSQL do this. RajSQL is a small version of that "somebody", written from scratch in C++, with nothing borrowed: no SQLite, no existing library. Every part (the SQL reader, the storage, the crash protection) was built line by line in this repository.

The cool part: RajSQL is compiled to **WebAssembly**, so the whole engine runs inside your web browser. You can try it right now without installing anything.

## 2. What is it for? (the honest answer)

Two purposes:

1. **Learning.** Databases feel like magic until you build one. RajSQL shows exactly what happens when you press ENTER on a query: the text gets split into words, turned into a plan, and your rows are found inside a tree structure stored on "pages" of a file. If you read this repository you will genuinely understand what `COMMIT` means on disk.
2. **A real, working tool.** It is not a toy. It stores real files, survives crashes, and runs real SQL. You can use it for small projects, prototypes, or to impress people in interviews by opening the hood.

## 3. How to use it in the browser (the easy way)

1. Open the demo page: **https://rajpalgelot-tech.github.io/RG-sql/**
2. Click **Load sample database** on the left. This creates three tables (students, courses, enrollments) filled with example data.
3. Click any example under **EXAMPLES** (for example "INNER JOIN"). It loads SQL into the editor.
4. Press **Run** (or Ctrl+Enter). The result table appears under **RESULTS**, with the time the engine took.
5. Type your own SQL in the editor and run it. A few things to try:
   - `SELECT * FROM students;`
   - `SELECT name, gpa FROM students WHERE gpa > 3.5 ORDER BY gpa DESC;`
   - `SELECT major, COUNT(*) FROM students GROUP BY major;` (not supported yet, this one will show the error message, which is also useful to see)
6. Your database is saved automatically in your browser (IndexedDB). Close the tab, come back tomorrow, and your tables are still there.

### The buttons on the left

| Button | What it does |
|---|---|
| **Load sample database** | Creates and fills the three example tables. |
| **Crash & recover demo** | The best part. See section 5 below. |
| **Persist now** | Saves the database to your browser immediately. |
| **Reset database** | Drops all tables and starts clean. |
| **Docs** | A short cheat sheet of the supported SQL. |

## 4. What SQL can it run?

RajSQL understands a practical subset of SQL:

```sql
-- make a table
CREATE TABLE marks (id INTEGER, subject TEXT, score REAL);

-- put rows in
INSERT INTO marks VALUES (1, 'math', 91.5), (2, 'physics', 84.0);

-- read rows back, with filters and sorting
SELECT subject, score FROM marks WHERE score >= 85 ORDER BY score DESC LIMIT 5;

-- combine two tables
SELECT s.name, c.title
FROM enrollments
JOIN students ON enrollments.student_id = students.id
JOIN courses  ON enrollments.course_id  = courses.id;

-- change and remove
UPDATE marks SET score = 95 WHERE id = 1;
DELETE FROM marks WHERE id = 2;

-- quick math over many rows
SELECT COUNT(*), AVG(score) FROM marks;
```

Notes:

- Every table automatically has a hidden `rowid` number, so rows are always findable.
- Types are `INTEGER`, `REAL` (decimals), and `TEXT`.
- If you type something RajSQL does not understand, it tells you clearly instead of crashing. Try it: type nonsense and press Run.

## 5. The crash demo (why this project is special)

Real databases promise something hard: **after a power cut, committed data must still be there.** RajSQL keeps that promise the same way big databases do, with a **write-ahead log (WAL)**:

1. Before touching the main database file, every changed page is written to a log file.
2. A "COMMIT" marker is added to the log. From this moment the data is safe.
3. Only then are the changes copied into the main file.
4. The log is cleared.

If the power dies between steps 2 and 3, nothing is lost: on startup the engine reads the log and replays everything that has a COMMIT marker.

Click **Crash & recover demo** and you will see this happen live:

1. 500 rows are inserted the normal way.
2. 200 more rows are inserted, but the program "loses power" right after the log was written, before the main file was updated.
3. The engine reopens, notices the log, replays it, and reports: **Recovered: 700/700 rows**.

No data was lost, exactly like a real database. That one green banner is the whole point of the storage half of this project.

## 6. Running it on your own computer

You need Docker (easiest) or any C++17 compiler.

```sh
# build + run the tests (no local compiler needed, uses Docker)
./build.sh

# build the browser version
./build.sh wasm
python3 -m http.server -d web 8080
# then open http://localhost:8080

# or use the command-line tool
./rajsql_cli mydata.db
```

The command-line version works like a mini terminal: type SQL, get tables back. `.help` lists the commands, `.quit` exits.

## 7. How it works (the 60-second version)

Your SQL text goes through four stages:

1. **Lexer**: splits the text into words and symbols ("SELECT", "students", "3.5").
2. **Parser**: turns the words into a structure the computer can act on (an AST).
3. **Executor**: walks that structure, finds your tables, and produces the rows.
4. **Storage**: rows live inside **pages** (4 KB blocks of a file). Each table is a **B+tree** so finding one row takes a few small steps instead of reading the whole file. The **WAL** (log file) makes commits survive power cuts.

The full tour, with diagrams, is in the [README](README.md).

## 8. What it does not do (yet)

Being honest is part of the project. RajSQL does not have: GROUP BY, LEFT JOIN, secondary indexes, and a few other things real databases have. Deleted rows leave small "tombstones" instead of being cleaned up immediately. The [roadmap](README.md#roadmap) lists what comes next.

## 9. Where to look in the code

| Curious about | Open this |
|---|---|
| How SQL text becomes tokens | `src/lexer.cpp` |
| How SQL becomes a plan | `src/parser.cpp` |
| How rows are stored on disk | `src/bptree.cpp`, `src/pager.cpp` |
| How crash safety works | `src/wal.cpp` |
| How a query actually runs | `src/database.cpp` |
| How it runs in the browser | `web/app.js` |

Every file has comments explaining the design decisions, so reading top to bottom is a small database course.
