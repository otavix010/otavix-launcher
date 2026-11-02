-- 01/11 (launcher 1.0.74): the language the player chose in the launcher (pt, en, es; empty on reports from older launchers), so the answer goes back in it
ALTER TABLE reports ADD COLUMN idioma TEXT NOT NULL DEFAULT '';
