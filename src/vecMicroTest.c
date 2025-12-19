/* Micro tests to isolate vec bugs */
#include "vec.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

/* Test data structure */
typedef struct testNode {
    int id;
    int visited;
} testNode;

/* Global counter for forEach tests */
static int g_forEach_count = 0;
static int g_forEach_sum = 0;

void countCallback(void *ptr, void *userData) {
    testNode *node = (testNode *)ptr;
    int *counter = (int *)userData;

    printf("  Visiting node %d (ptr=%p)\n", node->id, ptr);
    node->visited++;
    (*counter)++;
    g_forEach_sum += node->id;
}

void testVecPtrForEach() {
    printf("\n=== Test vecPtrForEach ===\n");

    vecError err;
    vecPtr *v = vecPtrNew(10, &err);
    assert(v != NULL && err == VEC_OK);

    /* Create test nodes */
    testNode nodes[5];
    for (int i = 0; i < 5; i++) {
        nodes[i].id = i + 1;
        nodes[i].visited = 0;
    }

    /* Add nodes to vector */
    for (int i = 0; i < 5; i++) {
        vecError err = vecPtrPush(v, &nodes[i]);
        printf("Push node %d: %s\n", nodes[i].id,
               err == VEC_OK ? "OK" : "FAIL");
        assert(err == VEC_OK);
    }

    printf("Vector count: %u\n", vecPtrCount(v));
    assert(vecPtrCount(v) == 5);

    /* Test forEach */
    int forEach_count = 0;
    g_forEach_sum = 0;
    printf("Calling vecPtrForEach...\n");
    vecPtrForEach(v, countCallback, &forEach_count);

    printf("forEach visited %d nodes\n", forEach_count);
    printf("Sum of IDs: %d (expected 15)\n", g_forEach_sum);

    assert(forEach_count == 5);
    assert(g_forEach_sum == 15);

    /* Verify all nodes were visited exactly once */
    for (int i = 0; i < 5; i++) {
        printf("Node %d visited %d times\n", nodes[i].id, nodes[i].visited);
        assert(nodes[i].visited == 1);
    }

    vecPtrFree(v);
    printf("✓ vecPtrForEach test PASSED\n");
}

void testVecPtrPushUnique() {
    printf("\n=== Test vecPtrPushUnique ===\n");

    vecError err;
    vecPtr *v = vecPtrNew(10, &err);
    assert(v != NULL && err == VEC_OK);

    testNode nodes[3];
    for (int i = 0; i < 3; i++) {
        nodes[i].id = i + 1;
        nodes[i].visited = 0;
    }

    /* First push should succeed */
    vecError err1 = vecPtrPushUnique(v, &nodes[0]);
    printf("First push of node %d: %s\n", nodes[0].id,
           err1 == VEC_OK
               ? "OK"
               : (err1 == VEC_ERROR_EXISTS ? "EXISTS" : "OTHER ERROR"));
    assert(err1 == VEC_OK);
    assert(vecPtrCount(v) == 1);

    /* Duplicate push should return EXISTS */
    vecError err2 = vecPtrPushUnique(v, &nodes[0]);
    printf("Duplicate push of node %d: %s\n", nodes[0].id,
           err2 == VEC_OK
               ? "OK"
               : (err2 == VEC_ERROR_EXISTS ? "EXISTS" : "OTHER ERROR"));
    assert(err2 == VEC_ERROR_EXISTS);
    assert(vecPtrCount(v) == 1);

    /* Different node should succeed */
    vecError err3 = vecPtrPushUnique(v, &nodes[1]);
    printf("Push of different node %d: %s\n", nodes[1].id,
           err3 == VEC_OK
               ? "OK"
               : (err3 == VEC_ERROR_EXISTS ? "EXISTS" : "OTHER ERROR"));
    assert(err3 == VEC_OK);
    assert(vecPtrCount(v) == 2);

    vecPtrFree(v);
    printf("✓ vecPtrPushUnique test PASSED\n");
}

void testVecPushUnique() {
    printf("\n=== Test vecPushUnique ===\n");

    vecError err;
    vec *v = vecNew(10, &err);
    assert(v != NULL && err == VEC_OK);

    /* First push should succeed */
    vecError err1 = vecPushUnique(v, 42);
    printf("First push of 42: %s\n",
           err1 == VEC_OK
               ? "OK"
               : (err1 == VEC_ERROR_EXISTS ? "EXISTS" : "OTHER ERROR"));
    assert(err1 == VEC_OK);
    assert(vecCount(v) == 1);

    /* Duplicate push should return EXISTS */
    vecError err2 = vecPushUnique(v, 42);
    printf("Duplicate push of 42: %s\n",
           err2 == VEC_OK
               ? "OK"
               : (err2 == VEC_ERROR_EXISTS ? "EXISTS" : "OTHER ERROR"));
    assert(err2 == VEC_ERROR_EXISTS);
    assert(vecCount(v) == 1);

    /* Different value should succeed */
    vecError err3 = vecPushUnique(v, 99);
    printf("Push of 99: %s\n",
           err3 == VEC_OK
               ? "OK"
               : (err3 == VEC_ERROR_EXISTS ? "EXISTS" : "OTHER ERROR"));
    assert(err3 == VEC_OK);
    assert(vecCount(v) == 2);

    vecFree(v);
    printf("✓ vecPushUnique test PASSED\n");
}

void testVecPtrGet() {
    printf("\n=== Test vecPtrGet ===\n");

    vecError err;
    vecPtr *v = vecPtrNew(10, &err);
    assert(v != NULL && err == VEC_OK);

    testNode nodes[3];
    for (int i = 0; i < 3; i++) {
        nodes[i].id = (i + 1) * 10;
        vecPtrPush(v, &nodes[i]);
    }

    /* Test getting each element */
    for (uint32_t i = 0; i < 3; i++) {
        void *retrieved = NULL;
        vecError err = vecPtrGet(v, i, &retrieved);
        printf("Get index %u: %s, id=%d\n", i, err == VEC_OK ? "OK" : "FAIL",
               retrieved ? ((testNode *)retrieved)->id : -1);
        assert(err == VEC_OK);
        assert(retrieved == &nodes[i]);
        assert(((testNode *)retrieved)->id == (i + 1) * 10);
    }

    vecPtrFree(v);
    printf("✓ vecPtrGet test PASSED\n");
}

int main() {
    printf("====================================\n");
    printf("Vec Micro Tests\n");
    printf("====================================\n");

    testVecPtrForEach();
    testVecPtrPushUnique();
    testVecPushUnique();
    testVecPtrGet();

    printf("\n====================================\n");
    printf("ALL MICRO TESTS PASSED!\n");
    printf("====================================\n");

    return 0;
}
