// RUN: %check_clang_tidy %s postgresql-hash-create-flags %t

// Mock HASHCTL struct and HASH_* flags from PostgreSQL hsearch.h
#define HASH_PARTITION  0x0001
#define HASH_SEGMENT    0x0002
#define HASH_DIRSIZE    0x0004
#define HASH_ELEM       0x0008
#define HASH_STRINGS    0x0010
#define HASH_BLOBS      0x0020
#define HASH_FUNCTION   0x0040
#define HASH_COMPARE    0x0080
#define HASH_KEYCOPY    0x0100
#define HASH_ALLOC      0x0200
#define HASH_CONTEXT    0x0400
#define HASH_SHARED_MEM 0x0800

typedef unsigned long Size;
typedef unsigned int (*HashValueFunc)(const void *key, Size keysize);
typedef int (*HashCompareFunc)(const void *key1, const void *key2, Size keysize);
typedef void *(*HashCopyFunc)(void *dest, const void *src, Size keysize);
typedef void *(*HashAllocFunc)(Size request);

typedef struct HASHHDR HASHHDR;

struct MemoryContextData;
typedef struct MemoryContextData *MemoryContext;

typedef struct HASHCTL {
  long num_partitions;
  long ssize;
  long dsize;
  long max_dsize;
  Size keysize;
  Size entrysize;
  HashValueFunc hash;
  HashCompareFunc match;
  HashCopyFunc keycopy;
  HashAllocFunc alloc;
  MemoryContext hcxt;
  HASHHDR *hctl;
} HASHCTL;

typedef struct HTAB HTAB;

HTAB *hash_create(const char *tabname, long nelem, HASHCTL *info, int flags);

// Test 1: Correct usage — no warnings expected.
void correct_usage(void) {
  HASHCTL ctl;
  ctl.keysize = sizeof(int);
  ctl.entrysize = sizeof(int);
  hash_create("good_hash", 16, &ctl, HASH_ELEM | HASH_BLOBS);
}

// Test 2: Missing HASH_ELEM flag.
void missing_hash_elem(void) {
  HASHCTL ctl;
  ctl.keysize = sizeof(int);
  ctl.entrysize = sizeof(int);
  hash_create("bad_hash", 16, &ctl, HASH_BLOBS);
  // CHECK-MESSAGES: :[[@LINE-1]]:3: warning: HASHCTL field 'entrysize' is set but corresponding flag 'HASH_ELEM' is not passed to hash_create [postgresql-hash-create-flags]
  // CHECK-MESSAGES: :[[@LINE-2]]:3: warning: HASHCTL field 'keysize' is set but corresponding flag 'HASH_ELEM' is not passed to hash_create [postgresql-hash-create-flags]
  // CHECK-MESSAGES: :[[@LINE-3]]:3: warning: HASH_ELEM flag is required for hash_create [postgresql-hash-create-flags]
}

// Test 3: Field set without corresponding flag.
void field_without_flag(MemoryContext ctx) {
  HASHCTL ctl;
  ctl.keysize = sizeof(int);
  ctl.entrysize = sizeof(int);
  ctl.hcxt = ctx;
  hash_create("bad_hash2", 16, &ctl, HASH_ELEM | HASH_BLOBS);
  // CHECK-MESSAGES: :[[@LINE-1]]:3: warning: HASHCTL field 'hcxt' is set but corresponding flag 'HASH_CONTEXT' is not passed to hash_create [postgresql-hash-create-flags]
}

// Test 4: Flag set without corresponding field.
void flag_without_field(void) {
  HASHCTL ctl;
  ctl.keysize = sizeof(int);
  ctl.entrysize = sizeof(int);
  hash_create("bad_hash3", 16, &ctl, HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);
  // CHECK-MESSAGES: :[[@LINE-1]]:38: warning: flag 'HASH_CONTEXT' is passed to hash_create but corresponding HASHCTL field 'hcxt' is not set [postgresql-hash-create-flags]
}

// Test 5: Mutually exclusive flags — HASH_STRINGS | HASH_BLOBS.
void mutually_exclusive_strings_blobs(void) {
  HASHCTL ctl;
  ctl.keysize = sizeof(int);
  ctl.entrysize = sizeof(int);
  hash_create("bad_hash4", 16, &ctl, HASH_ELEM | HASH_STRINGS | HASH_BLOBS);
  // CHECK-MESSAGES: :[[@LINE-1]]:38: warning: mutually exclusive flags 'HASH_STRINGS' and 'HASH_BLOBS' are both passed to hash_create [postgresql-hash-create-flags]
}

// Test 6: Mutually exclusive flags — HASH_BLOBS | HASH_FUNCTION.
void mutually_exclusive_blobs_function(HashValueFunc fn) {
  HASHCTL ctl;
  ctl.keysize = sizeof(int);
  ctl.entrysize = sizeof(int);
  ctl.hash = fn;
  hash_create("bad_hash5", 16, &ctl, HASH_ELEM | HASH_BLOBS | HASH_FUNCTION);
  // CHECK-MESSAGES: :[[@LINE-1]]:38: warning: mutually exclusive flags 'HASH_BLOBS' and 'HASH_FUNCTION' are both passed to hash_create [postgresql-hash-create-flags]
}

// Test 7: Correct usage with HASH_CONTEXT.
void correct_with_context(MemoryContext ctx) {
  HASHCTL ctl;
  ctl.keysize = sizeof(int);
  ctl.entrysize = sizeof(int);
  ctl.hcxt = ctx;
  hash_create("good_hash2", 16, &ctl, HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);
}

// Test 8: Correct usage with HASH_FUNCTION.
void correct_with_function(HashValueFunc fn) {
  HASHCTL ctl;
  ctl.keysize = sizeof(int);
  ctl.entrysize = sizeof(int);
  ctl.hash = fn;
  hash_create("good_hash3", 16, &ctl, HASH_ELEM | HASH_FUNCTION);
}

// Test 9: Correct usage with designated initializer — no warnings expected.
void correct_designated_init(void) {
  HASHCTL ctl = {
      .keysize = sizeof(int),
      .entrysize = sizeof(int),
  };
  hash_create("good_hash4", 16, &ctl, HASH_ELEM | HASH_BLOBS);
}

// Test 10: Designated initializer with field set but flag missing.
void designated_init_field_without_flag(MemoryContext ctx) {
  HASHCTL ctl = {
      .keysize = sizeof(int),
      .entrysize = sizeof(int),
      .hcxt = ctx,
  };
  hash_create("bad_hash6", 16, &ctl, HASH_ELEM | HASH_BLOBS);
  // CHECK-MESSAGES: :[[@LINE-1]]:3: warning: HASHCTL field 'hcxt' is set but corresponding flag 'HASH_CONTEXT' is not passed to hash_create [postgresql-hash-create-flags]
}
