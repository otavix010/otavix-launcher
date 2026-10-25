-- What goes along with a report: the game's log and settings (sent inside the report) and the player's pictures (each
-- in a request of its own, right after). Kept in the database: the mail through Resend is optional.
CREATE TABLE IF NOT EXISTS anexos (
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  report INTEGER NOT NULL,
  nome TEXT NOT NULL,
  tipo TEXT NOT NULL,          -- image/jpeg, image/png, text/plain
  tamanho INTEGER NOT NULL,
  dados BLOB NOT NULL          -- under D1's 2 MB a value
);
CREATE INDEX IF NOT EXISTS anexos_report ON anexos (report);
