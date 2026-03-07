// RUN: %check_clang_tidy %s postgresql-pfree-null %t

typedef unsigned long Size;
typedef void *MemoryContext;

// Mock pfree and palloc
void pfree(void *pointer);
void *palloc(Size size);

// Test 1: Direct NULL literal
void test_direct_null(void) {
  pfree((void *)0);
  // CHECK-MESSAGES: :[[@LINE-1]]:3: warning: calling 'pfree' with a NULL argument; unlike free(), pfree() does not accept NULL
}

// Test 2: NULL macro
#define NULL ((void *)0)

void test_null_macro(void) {
  pfree(NULL);
  // CHECK-MESSAGES: :[[@LINE-1]]:3: warning: calling 'pfree' with a NULL argument; unlike free(), pfree() does not accept NULL
}

// Test 3: Ternary with NULL branch (true)
void test_ternary_true_null(int cond, void *p) {
  pfree(cond ? NULL : p);
  // CHECK-MESSAGES: :[[@LINE-1]]:3: warning: calling 'pfree' with a potentially NULL argument; unlike free(), pfree() does not accept NULL
}

// Test 4: Ternary with NULL branch (false)
void test_ternary_false_null(int cond, void *p) {
  pfree(cond ? p : NULL);
  // CHECK-MESSAGES: :[[@LINE-1]]:3: warning: calling 'pfree' with a potentially NULL argument; unlike free(), pfree() does not accept NULL
}

// Test 5: Variable initialized to NULL, never reassigned
void test_var_null(void) {
  char *p = NULL;
  pfree(p);
  // CHECK-MESSAGES: :[[@LINE-1]]:3: warning: calling 'pfree' with a NULL argument; unlike free(), pfree() does not accept NULL
}

// Test 6: Variable assigned NULL, never reassigned
void test_var_assigned_null(void) {
  char *p;
  p = NULL;
  pfree(p);
  // CHECK-MESSAGES: :[[@LINE-1]]:3: warning: calling 'pfree' with a NULL argument; unlike free(), pfree() does not accept NULL
}

// Test 7: No warning - guarded with if (ptr)
void test_guarded(char *p) {
  if (p)
    pfree(p);
}

// Test 8: No warning - guarded with if (ptr != NULL)
void test_guarded_ne(char *p) {
  if (p != NULL)
    pfree(p);
}

// Test 9: No warning - guarded with compound body
void test_guarded_compound(char *p) {
  if (p) {
    pfree(p);
  }
}

// Test 10: No warning - variable reassigned after NULL
void test_reassigned(void) {
  char *p = NULL;
  p = (char *)palloc(64);
  pfree(p);
}

// Test 11: No warning - normal usage
void test_normal(void) {
  char *p = (char *)palloc(64);
  pfree(p);
}

// Test 12: No warning - function parameter (not definitely NULL)
void test_param(void *p) {
  pfree(p);
}

// Test 13: No warning - non-local variable (global)
char *global_ptr = NULL;
void test_global_null(void) {
  pfree(global_ptr);
}

// Test 14: No warning - variable reassigned inside nested block
void test_nested_reassign(int n) {
  char *p = NULL;
  if (n > 0) {
    p = (char *)palloc(64);
  }
  if (n > 0)
    pfree(p);
}

// Test 15: No warning - variable address passed to a function
void get_buffer(char **out);
void test_address_taken(void) {
  char *p = NULL;
  get_buffer(&p);
  pfree(p);
}
