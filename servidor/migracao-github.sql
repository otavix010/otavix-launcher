-- GitHub's answers for the launchers, kept a few minutes (and served stale while GitHub refuses): see github() in
-- worker.js.
CREATE TABLE IF NOT EXISTS github (
  caminho TEXT PRIMARY KEY,    -- the API path asked, with its query
  status INTEGER NOT NULL,
  corpo TEXT NOT NULL,
  quando INTEGER NOT NULL
);
