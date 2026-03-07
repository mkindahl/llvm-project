.. title:: clang-tidy - postgresql-hash-create-flags

postgresql-hash-create-flags
============================

Checks that calls to PostgreSQL's ``hash_create()`` have consistent ``HASHCTL``
field assignments and ``HASH_*`` flags.

Mismatches between the flags passed to ``hash_create()`` and the fields actually
set on the ``HASHCTL`` struct are a common source of bugs — silently reading
uninitialized memory or ignoring intended configuration. This check catches these
at compile time.

The check analyzes each call to ``hash_create()`` by:

1. Extracting the ``HASH_*`` macro names from the flags argument (4th parameter).
2. Walking backward through the enclosing scope to find field assignments on the
   ``HASHCTL`` variable (3rd parameter).
3. Comparing the assigned fields against the flags to detect inconsistencies.

Diagnostics
-----------

The check detects the following issues:

- **Missing HASH_ELEM**: The ``HASH_ELEM`` flag is required for all
  ``hash_create()`` calls (since PostgreSQL 13).

- **Field set without flag**: A ``HASHCTL`` field is assigned but the
  corresponding flag is not passed, so the value will be silently ignored.

- **Flag set without field**: A flag is passed but the corresponding
  ``HASHCTL`` field is not assigned, so ``hash_create()`` will read
  uninitialized memory.

- **Mutually exclusive flags**: ``HASH_STRINGS``, ``HASH_BLOBS``, and
  ``HASH_FUNCTION`` are mutually exclusive key-type strategies. Passing more
  than one is an error.

Field-to-Flag Mapping
---------------------

.. csv-table::
   :header: "HASHCTL Field(s)", "Required Flag"

   "``num_partitions``", "``HASH_PARTITION``"
   "``ssize``", "``HASH_SEGMENT``"
   "``dsize``, ``max_dsize``", "``HASH_DIRSIZE``"
   "``keysize``, ``entrysize``", "``HASH_ELEM``"
   "``hash``", "``HASH_FUNCTION``"
   "``match``", "``HASH_COMPARE``"
   "``keycopy``", "``HASH_KEYCOPY``"
   "``alloc``", "``HASH_ALLOC``"
   "``hcxt``", "``HASH_CONTEXT``"
   "``hctl``", "``HASH_SHARED_MEM``"

Example
-------

.. code-block:: c

   HASHCTL ctl;
   ctl.keysize = sizeof(Oid);
   ctl.entrysize = sizeof(MyEntry);
   ctl.hcxt = CurrentMemoryContext;

   // Warning: HASHCTL field 'hcxt' is set but corresponding flag
   // 'HASH_CONTEXT' is not passed to hash_create
   hash = hash_create("my_hash", 64, &ctl, HASH_ELEM | HASH_BLOBS);

   // OK: all flags match assigned fields
   hash = hash_create("my_hash", 64, &ctl,
                       HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);

Limitations
-----------

The analysis is intraprocedural — it only examines field assignments in the same
compound statement as the ``hash_create()`` call. If the ``HASHCTL`` variable is
initialized via a helper function or macro (other than direct member assignment),
those fields will not be detected. This covers the vast majority of PostgreSQL
usage patterns.

When a ``HASHCTL`` variable is reused across multiple ``hash_create()`` calls,
the check resets its knowledge of assigned fields at each prior call. Fields that
implicitly carry over from a previous call will not be seen.

Options
-------

.. option:: HashCreateFunction

   The name of the function to match. Default is ``hash_create``. This can be
   changed to match wrapper functions that have the same signature.
