.. title:: clang-tidy - postgresql-pfree-null

postgresql-pfree-null
=====================

Warns when ``pfree()`` is called with a NULL or potentially NULL argument.

Unlike the standard C ``free()`` function, PostgreSQL's ``pfree()`` does not
accept NULL pointers. Passing NULL to ``pfree()`` will cause a crash. This
check catches such cases at compile time.

Diagnostics
-----------

The check detects the following cases:

- **Direct NULL literal**: ``pfree(NULL)`` or ``pfree((void *)0)``.

- **Ternary with NULL branch**: ``pfree(cond ? ptr : NULL)`` where either
  branch of a conditional expression is NULL.

- **Variable definitely NULL**: A local variable is initialized or assigned
  NULL and never reassigned before being passed to ``pfree()``.

The check suppresses warnings when the ``pfree()`` call is inside a NULL guard:

.. code-block:: c

   if (ptr)
       pfree(ptr);        // No warning

   if (ptr != NULL)
       pfree(ptr);        // No warning

Example
-------

.. code-block:: c

   // Warning: calling 'pfree' with a NULL argument
   pfree(NULL);

   // Warning: calling 'pfree' with a potentially NULL argument
   pfree(cond ? ptr : NULL);

   // Warning: calling 'pfree' with a NULL argument
   char *p = NULL;
   pfree(p);

   // OK: variable reassigned before pfree
   char *q = NULL;
   q = palloc(64);
   pfree(q);

Limitations
-----------

The definitely-NULL analysis is intraprocedural and only examines the enclosing
compound statement. It tracks assignments and initializations of local variables
but does not follow values through function calls, pointer aliasing, or control
flow branches (other than recognizing NULL guards around the ``pfree()`` call
itself).

Options
-------

.. option:: PfreeFunction

   The name of the function to match. Default is ``pfree``. This can be changed
   to match wrapper functions or project-specific variants.
