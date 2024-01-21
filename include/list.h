#ifndef LIST_H
#define LIST_H

#include <stddef.h>
#include <stdio.h>

struct DList {
    DList *prev = NULL;
    DList *next = NULL;
};

void dlist_init(DList *node);
bool dlist_empty(DList *node);
void dlist_detach(DList *node);
void dlist_insert_before(DList *target, DList *rookie);

#endif // LIST_H