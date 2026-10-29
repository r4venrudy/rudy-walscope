# Controlled SQLite scenario

`make_capture.py NEW_DIRECTORY` creates an invented workload in its own temporary directory. It makes two tables, inserts a seed/static note, checkpoints, and saves a pre-WAL base. Three later commits insert records, update/delete records, and add an index. It copies the idle capture before closing its own connection, then writes expected query states.

Source checkouts contain only the generator. Native release packages generate `synthetic/base.sqlite`, `capture.wal`, `expected.json`, an example recovered image, and evidence reports on the corresponding native CI runner. SQLite writer versions and WAL salts may differ across runners; the expected SQL states are fixed. These are deliberate synthetic data, not sample user captures.

Try inspection, page 2 history, recovery at commits 1/2/3, and a commit 1-to-latest page comparison using the README commands. The static-notes table deliberately leaves an unchanged base page, so this scenario requires the supplied pre-WAL base and its explicit assertion. A separate test covers an image wholly contained in its WAL.

The tool never contacts network targets or opens the evidence through SQLite. The generator and test oracle use only databases they create or reconstructed working images. Keep generated captures out of Git; the ignore rules also help prevent accidental publication of private databases.
